# Pinout — ESP32 AI Recorder

> Version: v0.2 | Updated: 2026-05-12

---

## ⚠️ GPIO Freeze Notice

> **These GPIOs are FROZEN.**
> Do NOT modify any assignment in this table without:
> 1. Updating this file
> 2. Updating all affected `.c` source files
> 3. An architecture review confirming no hardware conflict

Modifying GPIO assignments breaks hardware in the field.
Any AI agent or developer MUST treat this table as read-only unless explicitly authorized.

---

## 1. Frozen GPIO Table

| Function    | GPIO | Direction | Peripheral  | Pull       | Notes                             |
|-------------|------|-----------|-------------|------------|-----------------------------------|
| **WS2812B** | 48   | OUT       | RMT         | None       | Single addressable RGB LED        |
| **Button**  | 0    | IN        | GPIO        | Pull-up    | Active-low; also boot mode pin    |
| **MIC_BCLK**| 4    | OUT       | I2S (BCLK)  | None       | I2S bit clock to INMP441          |
| **MIC_WS**  | 5    | OUT       | I2S (WS)    | None       | I2S word select / LRCLK to INMP441|
| **MIC_DIN** | 6    | IN        | I2S (DIN)   | None       | I2S data from INMP441             |
| **SD_CS**   | 10   | OUT       | SPI (CS)    | Pull-up    | TF card chip select               |
| **SD_MOSI** | 11   | OUT       | SPI (MOSI)  | None       | SPI data to TF card               |
| **SD_SCK**  | 12   | OUT       | SPI (SCLK)  | None       | SPI clock to TF card              |
| **SD_MISO** | 13   | IN        | SPI (MISO)  | Pull-up    | SPI data from TF card             |

---

## 2. Used GPIO Summary (by GPIO number)

```
GPIO 0   → Button (active-low, internal pull-up)
GPIO 4   → INMP441 BCLK
GPIO 5   → INMP441 WS (LRCLK)
GPIO 6   → INMP441 SD (data)
GPIO 10  → TF Card CS
GPIO 11  → TF Card MOSI
GPIO 12  → TF Card SCK
GPIO 13  → TF Card MISO
GPIO 48  → WS2812B DIN (RMT)
GPIO 1   → Battery ADC (ADC1_CH0) [see note below]
```

---

## 3. Battery ADC

| Signal     | GPIO | ADC Unit | Channel      | Attenuation  |
|------------|------|----------|--------------|--------------|
| VBAT_ADC   | 1    | ADC1     | ADC_CHANNEL_0| ADC_ATTEN_DB_12 |

Hardware: 100kΩ + 100kΩ voltage divider (ratio = 2.0×).
Full voltage: 4.2V → ADC reads ~2.1V. Empty: 3.3V → ADC reads ~1.65V.

> Battery ADC GPIO (GPIO1) is NOT in the frozen table because it is configured via `battery_config_t` at runtime.
> Changing it requires only a firmware config change, not hardware re-wiring (voltage divider remains on same pin).

---

## 4. SPI Bus Configuration

| Parameter    | Value         |
|--------------|---------------|
| SPI Host     | SPI2_HOST     |
| Max Freq     | 20 MHz (SDMMC_FREQ_DEFAULT) |
| DMA Channel  | SPI_DMA_CH_AUTO |
| MOSI         | GPIO11        |
| MISO         | GPIO13        |
| SCLK         | GPIO12        |
| CS           | GPIO10        |

---

## 5. I2S Configuration

| Parameter       | Value                          |
|-----------------|-------------------------------|
| I2S Port        | I2S_NUM_AUTO (auto-assigned)  |
| BCLK            | GPIO4                          |
| WS (LRCLK)      | GPIO5                          |
| DIN (data in)   | GPIO6                          |
| DOUT            | Not used (RX only)            |
| MCLK            | Not used (INMP441 has internal clock) |
| Sample Rate     | 16000 Hz                       |
| Bit Width       | 16-bit                         |
| Slot Mode       | Mono (left channel)           |
| Protocol        | I2S Philips (bit_shift=true)  |

---

## 6. Free GPIOs

The following GPIOs are currently unassigned and available for future use:

```
GPIO 2, 3, 7, 8, 9, 14, 15, 16, 17, 18, 19, 20, 21,
GPIO 35, 36, 37, 38, 39, 40, 41, 42, 43(TX), 44(RX), 45, 46, 47
```

> GPIO 43/44 are UART0 TX/RX (used by serial monitor). Reserve for debug use.
> GPIO 45 and 46 have boot-time strapping behavior — use with caution.

---

## 7. Conflict Warnings

| GPIO | Warning |
|------|---------|
| 0    | Boot mode pin. If held LOW at reset, ESP32 enters download mode. |
| 45   | Strapping pin: controls voltage of VDD_SPI. Avoid driving LOW at boot. |
| 46   | Strapping pin. Avoid using for general output. |
| 43/44| UART0 default pins. Serial monitor will not work if reassigned. |
