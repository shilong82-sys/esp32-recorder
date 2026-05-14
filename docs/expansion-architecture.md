# Expansion Architecture — ESP32 AI Recorder → AI Voice Terminal Platform

> **Version:** v0.1 | **Updated:** 2026-05-13
> **Scope:** Architecture planning only — no runtime implementation
> **Principle:** Build the platform foundation now; implement capabilities incrementally.

---

## 1. Platform Philosophy

### 1.1 System Goal

This device is **not** a "voice recorder."

It is a **low-power AI Voice Terminal Platform** — a programmable, extensible voice interaction node that can evolve from a simple recorder into a full AI assistant over 2–3 years.

### 1.2 Core Strategy: Coreboard + Expansion Capability

We deliberately **do not** integrate all future modules today. Instead:

- The **core board** handles what is verified and stable right now
- The **platform architecture** reserves resources for future capability layers
- Complexity must **evolve incrementally**, not explode in one release

### 1.3 Key Principles

| Principle | Meaning |
|-----------|---------|
| **Resourced, not pre-loaded** | Reserve interfaces and power headroom; do not solder future modules yet |
| **Layered evolution** | Add one capability layer at a time (audio output → network → display → sensors) |
| **Power-aware design** | Every future module has an explicit peak current budget |
| **Mechanical flexibility** | Favor daughterboards and connectors over permanent soldering |

---

## 2. Future Capability Layers

### 2.1 Audio Layer

| Capability | Description | Priority |
|------------|-------------|----------|
| **I2S TX (Speaker)** | Add audio output via MAX98357A or I2S DAC + Class D amp | High |
| **TTS** | Text-to-speech playback (AI responses, status prompts) | High |
| **Full Duplex Audio** | Simultaneous RX + TX for AI conversation | Medium |
| **Audio Codec** | Compress recordings (Opus, SPEEX) before upload | Low |
| **Wake Word Engine** | Local wake-word detection (e.g., "Hey ESP32") | Medium |
| **DSP / AEC** | Acoustic echo cancellation, noise suppression | Low |

### 2.2 Connectivity Layer

| Capability | Description | Priority |
|------------|-------------|----------|
| **WiFi (current)** | STA mode for upload; AP mode for provisioning | Done |
| **BLE** | Low-power provisioning, device discovery | Low |
| **4G / LTE** | Cellular backup for outdoor/offline-WiFi use | Medium |
| **GNSS** | Location tagging of recordings | Low |

### 2.3 Human Interaction Layer

| Capability | Description | Priority |
|------------|-------------|----------|
| **OLED Display** | I2C 128×64 OLED for status, transcript preview, menu | High |
| **Haptic Motor** | Vibration feedback for confirmation and alerts | Medium |
| **Additional Buttons** | More tactile controls (up/down/confirm) | Medium |
| **TFT / e-Paper** | Larger display for richer UI (future phase) | Low |

### 2.4 Sensor Layer

| Capability | Description | Priority |
|------------|-------------|----------|
| **IMU (6-axis)** | Activity detection, gesture recognition | Low |
| **Ambient Light Sensor** | Auto LED brightness, display backlight | Low |
| **Proximity Sensor** | Auto-wake on approach | Low |
| **Environmental Sensing** | Temperature, humidity (for niche recording use cases) | Low |

### 2.5 Expansion Layer

| Capability | Description | Priority |
|------------|-------------|----------|
| **Daughterboard Connector** | Pogo pins or mezzanine for add-on modules | High |
| **External Module Bus** | I2C/SPI expansion header for custom sensors | Medium |
| **FPC Connector** | Flexible print for off-board display or camera | Low |

### 2.6 Debug & Recovery Layer

| Capability | Description | Priority |
|------------|-------------|----------|
| **Safe Mode** | Boot into minimal mode if firmware update fails | Medium |
| **OTA Rollback** | Automatically revert to previous firmware on boot failure | Medium |
| **Recovery UART** | Dedicated serial debug port (separate from UART0 console) | Low |
| **JTAG/SWD** | Hardware debugging via dedicated debug header | Low |

---

## 3. Reserved Interfaces

> ⚠️ **These interfaces are FROZEN for current use but RESERVED for future expansion.**
> Do NOT reassign these GPIOs or buses to new peripherals without architecture review.

| Interface | Current GPIO(s) | Future Purpose | Status |
|-----------|----------------|----------------|--------|
| **I2C (main)** | GPIO1 / GPIO2 (not yet used) | OLED display, sensor expansion, RTC | Reserved |
| **UART2** | GPIO43 / GPIO44 (not yet used) | 4G module, GNSS | Reserved |
| **I2S TX** | (future channel on same I2S controller) | Speaker output, MAX98357A | Reserved |
| **SPI extra CS** | GPIO9 or GPIO14 | External flash, second sensor | Reserved |
| **RMT (channel 1)** | (not yet used) | Additional WS2812B strips or IR | Reserved |
| **Expansion GPIO** | GPIO15, GPIO16, GPIO17, GPIO18 | Future digital I/O, daughterboard | Reserved |
| **Analog (ADC2)** | GPIO21 (not yet used) | Environmental sensor, light sensor | Reserved |

---

## 4. Audio Expansion Strategy

### 4.1 Current State

- **I2S RX only**: INMP441 microphone on I2S0, 16kHz mono, 16-bit
- **No audio output**: System is strictly one-way (record only)

### 4.2 Future: I2S Full Duplex

The ESP32-S3 I2S controller supports simultaneous RX and TX on the same controller with shared clocks. This enables:

- **TTS playback**: AI responses spoken back to the user
- **Prompt tones**: Confirmation beep, recording start/stop chimes
- **AI voice assistant**: True back-and-forth conversation
- **PTT intercom**: Push-to-talk communication mode

### 4.3 Recommended Speaker Output Chain

```
Option A: MAX98357A (I2S in, Class D out, 3W)
  ESP32 I2S TX → MAX98357A → Speaker

Option B: I2S DAC + Class D Amp
  ESP32 I2S TX → PCM5102A → PAM8610 → Speaker
```

| Component | Pro | Con |
|-----------|-----|-----|
| MAX98357A | Single chip, no MCLK needed, cheap | Fixed gain, mono only |
| PCM5102A + PAM8610 | Stereo, higher quality | More components, MCLK needed |

**Recommendation for Phase 2:** MAX98357A (mono speaker for voice), upgrade to stereo DAC later if needed.

### 4.4 Wake Word Considerations

Wake-word engines (e.g., TensorFlow Lite Micro, ESP-Skainet) require:

- ~100KB additional RAM
- Dedicated audio processing task
- Potential conflict with recording task (must serialize access to `audio_read()`)

**Recommendation:** Implement after WAV pipeline (v0.2) and upload pipeline (v0.3) are stable.

---

## 5. Network Expansion Strategy

### 5.1 Current Architecture

```
┌─────────────┐
│  uploader   │  ← business logic
└──────┬──────┘
       │ esp_http_client
       ▼
    WiFi STA
```

### 5.2 Future: Network Abstraction Layer

```
┌─────────────┐
│  uploader   │  ← business logic (no WiFi dependency)
└──────┬──────┘
       │
┌──────▼──────┐
│network_mgr  │  ← abstraction layer
└──────┬──────┘
       │
┌──────▼──────┐
│  transport  │  ← pluggable backend
└──────┬──────┘
   ┌───┴───┐
   │       │
┌──▼──┐ ┌──▼──┐
│ WiFi│ │ 4G  │
└─────┘ └─────┘
```

**Key principle:** `uploader` must never call `wifi_*` functions directly. It calls `network_*` abstraction APIs.

### 5.3 4G Module Considerations (Important)

Adding 4G introduces significant complexity:

| Challenge | Impact |
|-----------|--------|
| **Power spike** | Up to 2A peak current — requires dedicated 4G power rail or robust bulk capacitance |
| **PPP stack** | Cellular requires PPP or NCM protocol — complex to implement reliably |
| **Reconnect logic** | Cellular handover, signal loss, retry must be handled gracefully |
| **UART ownership** | UART2 must be exclusive to 4G; conflicts with debug UART |
| **EMI** | 4G RF emissions near ESP32 antenna can degrade WiFi performance |
| **Data cost** | Recording uploads over cellular can be expensive — need compression first |

**Recommendation for Phase 3:** After audio output and compression are implemented.

### 5.4 GNSS (Location Tagging)

GNSS requires:
- UART2 (or shared with 4G)
- Active antenna with clear sky view
- Additional power budget
- Post-processing to embed GPS coordinates in WAV metadata

**Recommendation:** Low priority. Address after 4G if needed.

---

## 6. Display & Interaction Strategy

### 6.1 Current State

- **LED only**: WS2812B driven by state machine (solid / blink / breath patterns)
- **Single button**: Click / double-click / long-press for control

### 6.2 Phase 2: I2C OLED (128×64)

**Recommended display:** SSD1306 128×64 I2C OLED

```
ESP32 GPIO1 (SDA) ──┐
                    ├──→ SSD1306 OLED
ESP32 GPIO2 (SCL) ──┘
```

| Display | Resolution | Interface | Current Draw | Effort |
|---------|------------|-----------|-------------|--------|
| SSD1306 OLED | 128×64 | I2C | ~20mA | Low |
| SH1106 OLED | 128×64 | I2C | ~20mA | Low |
| ST7789 TFT | 240×320 | SPI | ~80mA | Medium |

**Recommendation:** Start with SSD1306 I2C OLED. Do not attempt SPI TFT in same phase as audio work.

### 6.3 Haptic Feedback

A small vibration motor (ERM or LRA) provides tactile confirmation:

```
ESP32 GPIO (PWM) → Transistor → Vibration Motor
```

- **ERM (Eccentric Rotating Mass):** Cheap, high current (~100mA)
- **LRA (Linear Resonant Actuator):** Lower current (~50mA), better feel, used in phones

**Recommendation:** Reserve GPIO for LRA driver. Implement after OLED (Phase 2).

### 6.4 Future: TFT / e-Paper

| Type | Pros | Cons |
|------|------|------|
| **TFT LCD** | Color, fast refresh, cheap | High current, SPI bus contention |
| **e-Paper** | Ultra low power, readable in sunlight | Slow refresh, no color (usually) |
| **e-Ink** | Same as e-Paper but higher quality | Expensive |

**Recommendation:** e-Paper for outdoor/low-power display use; TFT for rich UI.

---

## 7. Camera Strategy

### 7.1 Statement

**Camera is a future capability. It is not under active development.**

### 7.2 Why Not Now

Camera integration introduces immediate complexity:

| Resource | Impact |
|----------|--------|
| **PSRAM** | JPEG encoding requires significant RAM; ESP32-S3 has 8MB but it must be shared |
| **DMA channels** | Camera parallel interface uses many DMA channels — conflicts possible |
| **WiFi coexistence** | Heavy camera traffic + WiFi upload simultaneously can saturate the bus |
| **Power** | Camera + WiFi TX + recording = very high peak current |
| **Software** | esp-camera driver is complex; frame timing is delicate |

### 7.3 Future: Daughterboard Strategy

```
┌──────────────────┐
│  Core Board      │   ← ESP32-S3 core (current device)
│  (this project)  │
└────────┬─────────┘
         │ FPC / Pogo pins
         ▼
┌──────────────────┐
│  Camera Module   │   ← Future daughterboard
│  (OV2640/ESP32-  │
│   CAM module)    │
└──────────────────┘
```

**Recommendation:** When camera is needed, design a separate daughterboard with its own power regulator and ESP32-C3 (or dedicated camera MCU). Do not integrate camera directly onto the main board until all other systems are stable.

---

## 8. Power Expansion Budget

### 8.1 Current System Power Profile

| Component | Typical Current | Peak Current |
|-----------|----------------|--------------|
| ESP32-S3 (WiFi idle) | ~40–80mA | — |
| ESP32-S3 (WiFi TX) | ~100–240mA | 300–500mA |
| INMP441 mic | ~1.5mA | — |
| WS2812B LED | ~20mA (full white) | 50mA |
| TF card (active) | ~30mA | 100mA |
| **System total (recording)** | **~150mA** | **~300mA** |

### 8.2 Future Module Power Budget

| Module | Typical Current | Peak Current |
|--------|----------------|--------------|
| **ESP32-S3 WiFi TX** | 100–240mA | 300–500mA |
| **4G LTE burst** | 100–500mA | **up to 2A** |
| **MAX98357A Speaker (3W)** | ~100–300mA | 500mA+ |
| **SSD1306 OLED** | ~10–20mA | 30mA |
| **ST7789 TFT** | ~30–80mA | 100–300mA |
| **Camera (OV2640)** | ~60–100mA | 150mA |
| **LRA Haptic Motor** | ~50mA | 80mA |
| **GNSS Module** | ~30–50mA | 100mA |
| **IMU (BMI160)** | ~3–10mA | — |

### 8.3 System Peak Current Budget

> ⚠️ **Target: ≥ 5V / 3A peak design budget for future expansion.**

| Scenario | Peak Current | Power (5V) |
|----------|-------------|------------|
| Current: Recording + WiFi TX | ~500mA | 2.5W ✅ |
| Phase 2: + OLED + Speaker | ~800mA | 4W ✅ |
| Phase 3: + 4G burst | ~2.5A | 12.5W ⚠️ needs attention |
| Phase 4: + Camera | ~3A | 15W ⚠️ borderline |

**Critical note:** Average current ≠ peak current. A battery rated at 500mAh can deliver 500mA for 1 hour, but **cannot** deliver 2A for 2.5 hours — it's limited by the cell's C-rate. Choose battery cells with sufficient C-rating for peak demands.

### 8.4 Power Rail Recommendations

| Rail | Voltage | Current | Purpose |
|------|---------|---------|---------|
| Main rail | 3.3V | 500mA continuous | ESP32, SD card, LED, mic |
| Display rail | 3.3V | 300mA | OLED, TFT (switchable via MOSFET) |
| RF rail | 3.8V | 2A peak | 4G module (separate LDO/switch) |
| Audio rail | 5V | 1A | Speaker amp (USB-C or boost converter) |

---

## 9. Mechanical Expansion Strategy

### 9.1 Why Mechanical Matters

Embedding every future module on the main board creates problems:

- **Soldering risk**: Reworking BGA or small-pitch packages destroys surrounding components
- **Volume cost**: 4-layer PCB with all components is expensive to manufacture in small runs
- **Obsolescence**: If one module becomes obsolete, the whole board is useless
- **Field repair**: Cannot replace a single failed module

### 9.2 Recommended Expansion Mechanics

| Method | Use Case | Complexity |
|--------|----------|--------|
| **Pogo pins** | High-density signal (SPI, I2S, GPIO) | Medium |
| **Mezzanine connector (Pin header)** | Lower-density signals (I2C, UART, power) | Low |
| **FPC (Flexible Print Cable)** | Display, camera (off-board placement) | Medium |
| **Standoff + screws** | Heavy components (battery, speaker) | Low |
| **Soldered headers (default)** | Development / prototype stage | Low |

### 9.3 Daughterboard Concept

```
┌─────────────────────────────────────────────────┐
│              ESP32 Recorder Core                │
│                                                  │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐           │
│  │ I2S mic │ │  SD     │ │  WiFi   │           │
│  └────┬────┘ └────┬────┘ └────┬────┘           │
│       │           │           │                 │
│       └───────────┴───────────┘                 │
│                    │                            │
│            Expansion Connector                  │
│            (I2C / UART / GPIO / Power)         │
└────────────┬──────────────────────┬────────────┘
             │                      │
    ┌────────▼────────┐   ┌────────▼────────┐
    │  Daughterboard   │   │  Daughterboard  │
    │  Audio + Display │   │  Camera + 4G     │
    │  (Phase 2)       │   │  (Phase 3)      │
    └──────────────────┘   └─────────────────┘
```

**Recommendation:** Design a 2×10 or 2×20 pin expansion header (0.1" pitch) on the core board with:
- I2C bus
- UART2
- 3.3V power
- 4 GPIO pins
- Optional: SPI

This allows daughterboards to be designed and tested independently.

---

## 10. GPIO Reservation Strategy

### 10.1 Platform GPIO Philosophy

A significant portion of ESP32-S3 GPIOs must remain **unallocated** to prevent future resource fragmentation. The following rules apply:

1. **Current frozen GPIOs** (in use): 0, 4, 5, 6, 10, 11, 12, 13, 48
2. **Reserved for future** (do not use): 1, 2, 9, 14, 15, 16, 17, 18, 21, 43, 44
3. **Debug only** (not for peripherals): 19, 20 (USB D+/D-)

### 10.2 Reserved GPIO Table

| GPIO | Reason Reserved | Future Use |
|------|----------------|-----------|
| GPIO1 | ADC1 channel 0 (battery sense) + I2C SDA | I2C OLED, sensor expansion |
| GPIO2 | I2C SCL (not yet routed) | I2C OLED, sensor expansion |
| GPIO9 | SPI flash CS (do not use) | Reserved |
| GPIO14 | Reserved for future SPI device | Second SPI peripheral |
| GPIO15 | Strapping pin (JTAG) | Expansion GPIO |
| GPIO16 | UART2 RX (not yet used) | 4G TX, GNSS RX |
| GPIO17 | UART2 TX (not yet used) | 4G RX, GNSS TX |
| GPIO18 | Available | Expansion GPIO, haptic motor |
| GPIO21 | ADC2 channel 0 | Ambient light sensor |
| GPIO43 | Available | Expansion GPIO |
| GPIO44 | Available | Expansion GPIO |
| GPIO19 | USB D+ | Debug only (do not allocate) |
| GPIO20 | USB D- | Debug only (do not allocate) |

### 10.3 GPIO Allocation Rules

| Rule | Description |
|------|-------------|
| **No strapping pin reuse** | GPIO0, GPIO45, GPIO46 are strapping — minimal use only |
| **Analog pins for analog** | GPIO1, GPIO2, GPIO21 reserved for ADC use |
| **USB pins reserved** | GPIO19/GPIO20 are USB — do not use for peripherals |
| **SPI flash protected** | GPIO9 is used by SPI flash CS internally |
| **I2C bus dedicated** | GPIO1/GPIO2 form the I2C bus — not available for GPIO |

---

## 11. Long-Term Evolution Path

### Phase 1: Voice Recorder (Current — v0.1)

- Single-button recording
- WAV → SD card
- LED status feedback
- Battery monitoring
- **Done**

### Phase 2: Offline AI Memory Device (v0.4+)

- Upload WAV to Mac server
- Whisper transcription
- Local transcript storage
- Wake-word detection (optional)
- **Status: Architecture reserved**

### Phase 3: PTT AI Assistant (Future)

- Add I2S TX + speaker output
- MAX98357A integration
- TTS playback of transcripts
- Push-to-talk AI query
- **Status: Architecture reserved**

### Phase 4: Continuous Voice Agent (Future)

- Always-on voice wake word
- Full duplex audio
- Cellular connectivity (4G)
- GPS location tagging
- Camera (photo capture on voice command)
- **Status: Architecture reserved**

### Phase 5: Platform Ecosystem (Long-term)

- Daughterboard ecosystem
- Third-party module compatibility
- Cloud sync beyond local Mac
- OTA firmware marketplace
- **Status: Vision only**

---

*Last updated: 2026-05-13*
*Document owner: AI engineering agent + shilong82-sys*
*Next review: Before Phase 2 implementation*
