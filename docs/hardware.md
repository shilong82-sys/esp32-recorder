# Hardware — ESP32 AI Recorder

> Version: v0.2 | Updated: 2026-05-12

---

## 1. Bill of Materials

| # | Component       | Part Number / Spec       | Qty | Notes                                     |
|---|-----------------|--------------------------|-----|-------------------------------------------|
| 1 | MCU Module      | ESP32-S3-DevKitC-1 N16R8 | 1   | 16MB Flash, 8MB PSRAM, USB-C              |
| 2 | Microphone      | INMP441                  | 1   | I2S MEMS, mono, 3.3V, bottom port        |
| 3 | TF Card Module  | Generic SPI micro-SD     | 1   | 3.3V level, SPI mode                     |
| 4 | TF Card         | ≥16GB micro-SD           | 1   | Must be formatted FAT32                   |
| 5 | LED             | WS2812B (5mm module)     | 1   | 5V data, addressable RGB                 |
| 6 | Button          | 6×6mm tactile            | 1   | Active-low with internal pull-up          |
| 7 | Battery         | 3.7V Li-ion              | 1   | ~500mAh or larger                        |
| 8 | Voltage Divider | 2× 100kΩ resistors       | 1   | For battery ADC measurement               |

---

## 2. Wiring Diagram (Text)

### 2.1 INMP441 Microphone

```
INMP441 Pin → ESP32-S3 GPIO
─────────────────────────────
VDD   → 3.3V
GND   → GND
BCLK  → GPIO4  (MIC_BCLK)
WS    → GPIO5  (MIC_WS / LRCLK)
SD    → GPIO6  (MIC_DIN / data output from mic)
L/R   → GND   (select left channel)
```

> **Note:** L/R pin tied to GND = left channel output. ESP32 receives on left channel (WS LOW).

### 2.2 SPI TF Card Module

```
SD Module Pin → ESP32-S3 GPIO
──────────────────────────────
VCC   → 3.3V
GND   → GND
CS    → GPIO10 (SD_CS)
MOSI  → GPIO11 (SD_MOSI)
SCK   → GPIO12 (SD_SCK)
MISO  → GPIO13 (SD_MISO)
```

> **Known issue (BUG-001):** Dupont wire connection is unstable. May cause `ESP_ERR_TIMEOUT (0x107)`.
> Soldering or using a dedicated SD card breakout board recommended for production.

### 2.3 WS2812B LED

```
WS2812B Pin → ESP32-S3
──────────────────────
5V    → 5V (or 3.3V for short distances)
GND   → GND
DIN   → GPIO48
```

> Driven by RMT peripheral. No external resistor needed for dev use.

### 2.4 Button

```
Button → ESP32-S3
─────────────────
One terminal  → GPIO0
Other terminal → GND
(Internal pull-up enabled in firmware)
```

> GPIO0 is also the ESP32 boot mode selection pin. Do not hold button during power-on.

### 2.5 Battery ADC

```
Battery+ → 100kΩ → GPIO1 (ADC1_CH0) → 100kΩ → GND
```

Voltage divider ratio: 2.0x. Full voltage: 4.2V. Empty voltage: 3.3V.

---

## 3. Power Budget (Estimated)

| State       | Estimated Current | Notes                            |
|-------------|------------------|----------------------------------|
| IDLE        | ~80 mA           | WiFi idle, I2S active            |
| RECORDING   | ~120 mA          | I2S active, SD write active      |
| UPLOADING   | ~150 mA          | WiFi TX active                   |
| DEEP SLEEP  | ~20 µA (target)  | Not yet implemented              |

With 500mAh battery: ~4 hours continuous recording (estimate, not measured).

---

## 4. I2S Microphone — INMP441 Technical Notes

- **Protocol:** I2S Philips standard (1-bit MSB delay after WS edge)
- **Data format:** 24-bit audio data, left-padded to 32-bit word
- **Sampling:** 16kHz, mono (left channel)
- **ESP-IDF API:** `i2s_new_channel()` + `i2s_channel_init_std_mode()` (v5 API)
- **Critical config:** `bit_shift = true` (handles Philips 1-bit delay)
- **Slot mask:** `I2S_STD_SLOT_LEFT` (receive left channel only)

```c
// Confirmed working configuration (audio.c)
i2s_std_slot_config_t slot_cfg = {
    .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
    .slot_mode      = I2S_SLOT_MODE_MONO,
    .slot_mask      = I2S_STD_SLOT_LEFT,
    .ws_width       = 16,
    .ws_pol         = false,
    .bit_shift      = true,   // CRITICAL for INMP441
    .left_align     = false,
    .big_endian     = false,
    .bit_order_lsb  = false,
};
```

---

## 5. SD Card — Requirements

| Parameter       | Requirement             |
|-----------------|-------------------------|
| Format          | FAT32 (NOT exFAT)       |
| Capacity        | Tested with 16GB        |
| Class / Speed   | Class 10 recommended    |
| Allocation unit | 16 KB (firmware default)|
| Mount point     | `/sdcard`               |
| SPI frequency   | 20 MHz (SDMMC_FREQ_DEFAULT) |

> exFAT is NOT supported by ESP-IDF FATFS without extra configuration.
> Format card with Windows "Format" tool or `mkfs.fat -F 32` on Linux/Mac.

---

## 6. Development Setup

- **Host OS:** macOS (Apple Silicon M-series)
- **IDE:** Any (VS Code + ESP-IDF extension recommended)
- **Serial monitor:** `firmware/scripts/monitor.sh` or `idf.py monitor`
- **Flash:** `firmware/scripts/flash.sh` or `idf.py flash`
- **ESP-IDF version:** v5.2.3 (local submodule at `esp-idf/`)
- **Python (Mac):** used for Mac server (`server/`)
- **Whisper:** mlx-whisper, Apple Silicon Metal GPU
