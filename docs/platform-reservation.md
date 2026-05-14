# Platform Reservation Rules — ESP32 AI Voice Terminal Platform

> **Version:** v0.1 | **Updated:** 2026-05-13
> **Scope:** Permanent resource reservation rules for future expansion
> **Principle:** A platform that cannot expand is a dead-end product.

---

## 1. Overview

This document defines the **hard reservation rules** that govern how GPIO, bus, power, mechanical, thermal, and RF resources are allocated in this project.

These rules exist to ensure that **future capability layers** (audio output, 4G, display, camera, sensors) can be added without breaking the existing system or re-soldering the board.

---

## 2. Reserved GPIO

### 2.1 Frozen GPIOs (In Active Use)

These GPIOs are allocated to current hardware and **must not be reassigned** without a full architecture review and a version-bumped `docs/pinout.md`.

| GPIO | Function | Component | Notes |
|------|----------|-----------|-------|
| GPIO0 | Button | button | Active-low, internal pull-up |
| GPIO4 | MIC_BCLK | audio | I2S bit clock output |
| GPIO5 | MIC_WS | audio | I2S word select / LRCLK |
| GPIO6 | MIC_DIN | audio | I2S data input from INMP441 |
| GPIO10 | SD_CS | storage | SPI chip select |
| GPIO11 | SD_MOSI | storage | SPI MOSI |
| GPIO12 | SD_SCK | storage | SPI clock |
| GPIO13 | SD_MISO | storage | SPI MISO |
| GPIO48 | WS2812B | led | RMT peripheral |
| ADC1_CH0 (GPIO1) | Battery sense | battery | ADC1 channel 0 |

### 2.2 Reserved GPIOs (Future Use Only)

These GPIOs are **not yet used** and are **reserved** for future expansion. They must not be allocated to new components during current phase development.

| GPIO | Reservation Reason | Priority Use |
|------|-------------------|--------------|
| GPIO1 | I2C SDA + battery ADC | I2C OLED, sensor expansion |
| GPIO2 | I2C SCL | I2C OLED, sensor expansion |
| GPIO9 | SPI flash CS (internal) | Reserved — do not use |
| GPIO14 | Extra SPI chip select | Second SPI device (future) |
| GPIO15 | Strapping + expansion | Daughterboard GPIO |
| GPIO16 | UART2 RX | 4G TX, GNSS RX |
| GPIO17 | UART2 TX | 4G RX, GNSS TX |
| GPIO18 | Expansion GPIO | Haptic motor, daughterboard |
| GPIO21 | ADC2 channel 0 | Ambient light, environmental |
| GPIO43 | Available | Expansion GPIO |
| GPIO44 | Available | Expansion GPIO |
| GPIO19 | USB D+ | Debug only — do not allocate |
| GPIO20 | USB D- | Debug only — do not allocate |
| GPIO45 | Strapping pin | Limited use only |
| GPIO46 | Strapping pin | Limited use only |

### 2.3 GPIO Rules

| Rule | Description |
|------|-------------|
| **Strapping pins are fragile** | GPIO0, GPIO45, GPIO46 determine boot mode. Only use after boot completes. |
| **USB pins are debug only** | GPIO19/GPIO20 are USB D+/D-. Do not route to peripherals. |
| **I2C bus is shared** | GPIO1/GPIO2 form the I2C bus. All I2C devices share these two lines (with unique addresses). |
| **SPI flash protection** | GPIO9 is the internal SPI flash CS. Never use this for other peripherals. |
| **Analog pins for analog** | GPIO1, GPIO21 are best used for ADC. Keep analog and digital switching separate. |

---

## 3. Reserved Buses

### 3.1 I2C Bus (Main)

| Property | Value |
|----------|-------|
| GPIO | GPIO1 (SDA), GPIO2 (SCL) |
| Speed | 400kHz (Fast Mode) |
| Voltage | 3.3V (do not level-shift without review) |
| Max devices | ~8 (limited by address space) |

**I2C devices in future phases:**

| Device | Address | Phase |
|--------|---------|-------|
| SSD1306 OLED | 0x3C | Phase 2 |
| SH1106 OLED | 0x3C | Phase 2 |
| BME280 sensor | 0x76 / 0x77 | Future |
| DS3231 RTC | 0x68 | Future |
| TSL2591 light sensor | 0x29 | Future |

**I2C address conflict rule:** Before adding any I2C device, scan the bus to verify no address conflict. Document all assigned I2C addresses in `docs/pinout.md`.

### 3.2 UART2 (Cellular / GNSS)

| Property | Value |
|----------|-------|
| GPIO | GPIO16 (RX), GPIO17 (TX) |
| Baud rate | 115200 (default for most 4G/GNSS modules) |
| Voltage | 3.3V TTL (4G modules often need 1.8V — check carefully) |

**⚠️ UART2 voltage warning:** Many 4G LTE modules (Quectel BC66, BC95) use 1.8V UART signaling. Direct connection to ESP32 3.3V GPIO can damage the module. Use a level shifter or verify the module's UART voltage tolerance.

### 3.3 SPI Bus (Current)

| Property | Value |
|----------|-------|
| Bus | HSPI (SPI2) |
| GPIO | GPIO10 (CS), GPIO11 (MOSI), GPIO12 (CLK), GPIO13 (MISO) |
| Speed | Up to 40MHz |
| Current device | TF card (SPI mode) |

**Future SPI devices:** One extra CS pin (GPIO14) is reserved on the SPI bus for a future SPI peripheral.

### 3.4 I2S (Audio)

| Property | Value |
|----------|-------|
| Controller | I2S0 |
| RX GPIO | GPIO4 (BCLK), GPIO5 (WS), GPIO6 (DIN) |
| TX GPIO | (Not yet routed) — reserved for future MAX98357A or DAC |

**I2S TX routing (future):**
```
I2S0 TX  →  GPIO38 (or configurable via matrix)
I2S0 BCLK →  (shared with RX if half-duplex, or separate)
I2S0 WS  →  (shared with RX if half-duplex, or separate)
```

---

## 4. Reserved Power Budget

### 4.1 System Power Budget

> ⚠️ **This is a hard constraint. Every new module must fit within the allocated power budget or require a separate power rail.**

| Rail | Voltage | Continuous Current | Peak Current | Allocated To |
|------|---------|-------------------|--------------|-------------|
| Main 3.3V | 3.3V | 500mA | 800mA | ESP32, SD card, LED, mic, future expansion |
| Analog 3.3V | 3.3V | 100mA | 200mA | ADC, sensors |
| Display rail | 3.3V | 200mA | 300mA | OLED / TFT (switchable) |
| RF rail | 3.8V | — | 2000mA | 4G module (separate LDO) |
| Audio rail | 5V | 500mA | 1000mA | Speaker amp (via USB-C or boost) |

### 4.2 Current System Load

| Component | Voltage | Current |
|-----------|---------|---------|
| ESP32-S3 (WiFi idle) | 3.3V | 40–80mA |
| ESP32-S3 (WiFi TX) | 3.3V | 100–240mA |
| INMP441 microphone | 3.3V | 1.5mA |
| WS2812B LED (active) | 5V → 3.3V via resistor | ~20mA |
| TF card (active) | 3.3V | 30–80mA |
| **Current total (recording)** | — | **~150–250mA** |

**Remaining headroom on main 3.3V rail:** ~250mA continuous, ~550mA peak.

### 4.3 Future Load Projections

| Phase | New Modules | Additional Current | Rail | Status |
|-------|-------------|-------------------|------|--------|
| Phase 2 | OLED + MAX98357A | +150mA | Main 3.3V + Audio 5V | ✅ Fits budget |
| Phase 3 | 4G LTE burst | +2000mA peak | RF rail 3.8V | ⚠️ Separate rail required |
| Phase 4 | Camera OV2640 | +150mA peak | Main 3.3V | ⚠️ Tight — monitor closely |
| Phase 4 | GNSS | +50mA | Main 3.3V | ✅ Fits budget |

### 4.4 Battery Cell Recommendation

| Requirement | Value |
|-------------|-------|
| Minimum capacity | 500mAh |
| Recommended capacity | 1000–2000mAh |
| Minimum C-rating | 2C (for 4G peak) |
| Recommended C-rating | 5C (for 4G + speaker + camera peak) |
| Nominal voltage | 3.7V Li-ion |
| Charging | USB-C with MCP73831 or TP4056 |

---

## 5. Reserved Mechanical Volume

### 5.1 Current Board Dimensions

The current ESP32-S3 dev board (generic module with headers) occupies approximately:

- **Length:** ~70mm (with headers)
- **Width:** ~30mm
- **Height:** ~20mm (including components)

### 5.2 Mechanical Reservation Rules

| Rule | Description |
|------|-------------|
| **No tall components on bottom** | SD card slot and battery must be accessible |
| **Clear WiFi antenna zone** | Keep metallic objects > 10mm from ESP32 antenna (on-board or external) |
| **USB-C placement** | USB-C must be on board edge for cable access |
| **Button accessibility** | Physical button must be accessible without disassembly |
| **Battery bay** | Reserve space for 18650 or 603450 LiPo cell |

### 5.3 Future Expansion Volume

For daughterboard stacking:

| Layer | Max Height | Purpose |
|-------|-----------|---------|
| Core board | 15mm | Current ESP32-S3 module |
| Daughterboard 1 | 10mm | OLED + MAX98357A |
| Daughterboard 2 | 15mm | 4G + GNSS (taller components) |
| Daughterboard 3 | 8mm | Camera (flat flex) |
| **Total stack** | **~48mm** | Must fit in enclosure design |

### 5.4 Enclosure Design Guidelines

| Guideline | Value |
|-----------|-------|
| Minimum wall thickness | 1.5mm (ABS/PLA) |
| Antenna keepout | 5mm from any metallic surface |
| Ventilation slots | If sealed, include 1–2mm vents for thermal relief |
| Battery access | Removable panel or snap-fit for battery replacement |

---

## 6. Thermal Reservation

### 6.1 Current Thermal Profile

| Component | Typical T° | Max T° | Notes |
|-----------|-----------|--------|-------|
| ESP32-S3 | 40–60°C (normal load) | 105°C | Keep below 80°C for longevity |
| INMP441 | Ambient + 5°C | 85°C | Keep away from heat sources |
| WS2812B | 30–45°C | 70°C | Can get hot at full white brightness |
| TF card | 30–50°C | 85°C | High-speed transfers raise temperature |

### 6.2 Future Thermal Budget

Adding future modules increases thermal load:

| Module | Heat Source | Impact |
|--------|-------------|--------|
| MAX98357A | Class D amp | 10–20°C rise at full volume |
| 4G LTE | RF PA + modem | 20–40°C rise during transmission burst |
| OLED | Display driver | 5–10°C rise at full brightness |

### 6.3 Thermal Design Rules

| Rule | Description |
|------|-------------|
| **No direct stacking** | Do not stack heat-generating modules directly on ESP32 thermal pad |
| **Thermal vias** | Any daughterboard should have thermal relief vias near heat sources |
| **Temperature monitoring** | ESP32 has internal temperature sensor — use it for thermal monitoring in v0.5+ |
| **Thermal runaway protection** | If internal ESP32 temp > 85°C, reduce WiFi TX power or enter light sleep |

### 6.4 Deep Sleep vs Active Thermal

| Mode | ESP32 T° | Notes |
|------|----------|-------|
| Active (recording) | 40–60°C | Depends on ambient |
| WiFi TX burst | +10–15°C above idle | Peak during upload |
| Deep sleep | Ambient + 2°C | Minimal self-heating |
| Light sleep | Ambient + 5°C | Between active and deep |

---

## 7. Antenna Keepout Planning

### 7.1 ESP32-S3 On-Board Antenna

The ESP32-S3-WROOM modules have an on-board PCB trace antenna. The antenna has a **keepout zone** that must be free of:

- Metallic objects (ground planes, battery, enclosures with metal)
- High-current traces
- Liquid (water in enclosure causes severe attenuation)

**Keepout zone (typical for WROOM-1N):**
- 15mm clearance above the antenna
- No ground plane within 5mm of antenna edges
- No conductive materials within 10mm

### 7.2 Antenna Rules

| Rule | Description |
|------|-------------|
| **No ground pour near antenna** | If using PCB ground pour, keep > 5mm from antenna region |
| **Battery placement** | Do not place LiPo battery directly above ESP32 antenna |
| **Metal enclosure warning** | Metal enclosures will block WiFi. Use plastic or add external antenna. |
| **External antenna option** | ESP32-S3-WROOM-1U has u.FL connector for external antenna — preferred for Phase 3+ if enclosure is metallic |
| **4G + WiFi coexistence** | If 4G is added, keep 4G antenna > 30mm from WiFi antenna to minimize interference |

### 7.3 RF Shielding Considerations

| Scenario | Recommendation |
|----------|---------------|
| WiFi only (current) | No RF shielding needed |
| 4G + WiFi | Consider RF shielding on 4G module to protect ESP32 WiFi sensitivity |
| Camera + WiFi | Camera parallel interface can generate EMI — consider shielding |

---

## 8. Expansion Connector Philosophy

### 8.1 Connector Design Principles

| Principle | Description |
|-----------|-------------|
| **Standard pitch** | 0.1" (2.54mm) pin header preferred — easy to source and solder |
| **Keyed connectors** | Use polarization notches to prevent reverse insertion |
| **Locking mechanism** | Screws or friction fit for vibration resistance |
| **Gold-plated contacts** | Prefer gold-plated headers for durability |
| **Cable management** | Plan cable routing in enclosure design early |

### 8.2 Recommended Expansion Header

**2×10 pin header (0.1" pitch):**

| Pin | Signal | Notes |
|-----|--------|-------|
| 1 | 3.3V | Power (shared with main rail) |
| 2 | 5V | Power (USB-C rail, for speaker amp) |
| 3 | GND | Ground |
| 4 | GND | Ground |
| 5 | GPIO1 (SDA) | I2C SDA |
| 6 | GPIO16 (UART2 RX) | UART RX |
| 7 | GPIO2 (SCL) | I2C SCL |
| 8 | GPIO17 (UART2 TX) | UART TX |
| 9 | GPIO18 | Expansion GPIO |
| 10 | GPIO14 | SPI extra CS |
| 11 | GPIO15 | Expansion GPIO (strapping — safe after boot) |
| 12 | GPIO43 | Expansion GPIO |
| 13 | GPIO44 | Expansion GPIO |
| 14 | GPIO21 | ADC2 channel 0 |
| 15 | GND | Ground |
| 16 | GND | Ground |
| 17 | (reserved) | — |
| 18 | (reserved) | — |
| 19 | (reserved) | — |
| 20 | (reserved) | — |

### 8.3 FPC Connector (For Display / Camera)

For off-board display and camera, use FPC (Flexible Printed Circuit) connectors:

| Type | Use Case | Notes |
|------|----------|-------|
| 0.5mm pitch FPC | OLED, small sensors | Low pin count |
| 1.0mm pitch FPC | Camera, TFT display | Higher pin count, more robust |

---

## 9. Summary: Hard Rules

These rules are **non-negotiable** and must be updated only through architecture review:

| Rule | Constraint |
|------|------------|
| **GPIO frozen** | No changes to GPIO0, 4, 5, 6, 10, 11, 12, 13, 48 without full architecture review |
| **I2C reserved** | GPIO1/GPIO2 reserved for I2C bus — do not repurpose for GPIO |
| **UART2 reserved** | GPIO16/GPIO17 reserved for UART2 (4G/GNSS) — do not use for GPIO |
| **Strapping pins** | GPIO0, GPIO45, GPIO46 available only after boot — do not rely on state at boot |
| **USB pins** | GPIO19/GPIO20 are USB D+/D- — not available for peripherals |
| **Power headroom** | Any new module adding > 200mA to main 3.3V rail requires a separate power rail |
| **4G power** | 4G module must have dedicated 3.8V rail — do not power from main 3.3V rail |
| **Antenna keepout** | No metallic objects within 15mm of ESP32 WiFi antenna |
| **Thermal budget** | ESP32 must stay below 80°C under all operating conditions |
| **SPI flash protection** | GPIO9 is internal flash CS — absolutely off-limits |
| **Expansion header** | All new expansion connectors must be documented in `docs/pinout.md` |

---

*Last updated: 2026-05-13*
*Document owner: AI engineering agent + shilong82-sys*
*Review frequency: Before each new phase implementation*
