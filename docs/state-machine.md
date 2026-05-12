# State Machine — ESP32 AI Recorder

> Version: v0.2 | Updated: 2026-05-12

---

## 1. States Overview

| State       | Code Name                  | Description                          |
|-------------|----------------------------|--------------------------------------|
| INIT        | `DEVICE_STATE_INIT`        | System initializing, modules loading |
| IDLE        | `DEVICE_STATE_IDLE`        | Ready, waiting for user input        |
| RECORDING   | `DEVICE_STATE_RECORDING`   | Actively capturing audio to SD card  |
| UPLOADING   | `DEVICE_STATE_UPLOADING`   | Sending WAV file to Mac server       |
| ERROR       | `DEVICE_STATE_ERROR`       | Fault condition, recovery in progress|
| SLEEP       | `DEVICE_STATE_SLEEP`       | Deep sleep / low-power standby       |

> **Note:** `PROCESSING` (WAV post-processing / trim) is reserved for future use.
> Currently, recording stops → file is complete → ready for upload. No intermediate processing state.

---

## 2. State Transition Diagram

```
                    ┌─────────────────────────────┐
                    │           INIT               │
                    │  (boot sequence running)     │
                    └────────────┬────────────────-┘
                                 │ init OK
                                 ▼
                    ┌─────────────────────────────┐
               ┌──▶│           IDLE               │◀────────────────┐
               │   │  (waiting, LED slow blink)   │                 │
               │   └──┬──────────────┬────────────┘                 │
               │      │ btn click    │ idle timeout                 │
               │      ▼              ▼                              │
               │  ┌────────────┐  ┌────────────┐                   │
               │  │ RECORDING  │  │   SLEEP    │──btn wakeup──────▶│
               │  │ (LED fast  │  │ (deep sleep│                   │
               │  │  blink)    │  │  20µA)     │                   │
               │  └────┬───────┘  └────────────┘                   │
               │       │ btn click / SD full / battery low          │
               │       ▼                                            │
               │  ┌────────────┐                                    │
               │  │ (file done)│                                    │
               │  │ check WiFi │                                    │
               │  └──┬─────────┘                                    │
               │     │ WiFi OK                                      │
               │     ▼                                              │
               │  ┌────────────┐   upload done                     │
               └──│ UPLOADING  │──────────────────────────────────▶│
                  │ (LED breath│                                    │
                  │  pattern)  │                                    │
                  └──┬─────────┘                                    │
                     │ upload failed                                │
                     ▼                                              │
                  ┌────────────┐   auto-recover / btn              │
                  │   ERROR    │──────────────────────────────────▶│
                  │ (LED fast  │                                    │
                  │  red blink)│                                    │
                  └────────────┘
```

---

## 3. Transition Rules

| From        | To          | Trigger                                           | Guard                        |
|-------------|-------------|---------------------------------------------------|------------------------------|
| INIT        | IDLE        | All modules initialized successfully               | —                            |
| INIT        | ERROR       | Critical init failure (e.g., SD card not found)   | —                            |
| IDLE        | RECORDING   | `EVENT_BUTTON_CLICKED`                            | State must be IDLE           |
| IDLE        | UPLOADING   | Files in pending queue AND WiFi connected         | Triggered by IDLE periodic check |
| IDLE        | SLEEP       | Idle timeout (60s default, configurable)          | No pending upload            |
| RECORDING   | IDLE        | `EVENT_BUTTON_CLICKED` (stop recording)           | State must be RECORDING      |
| RECORDING   | ERROR       | SD card write failure                             | —                            |
| RECORDING   | IDLE        | SD card nearly full (<1MB remaining)              | Auto-stop                    |
| RECORDING   | IDLE        | Battery critical (<5%)                            | Auto-stop                    |
| UPLOADING   | IDLE        | Upload complete (`EVENT_UPLOAD_DONE`)             | —                            |
| UPLOADING   | ERROR       | Upload failed, retries exhausted                  | After 3 retries              |
| ERROR       | IDLE        | Auto-recovery success OR `EVENT_BUTTON_CLICKED`   | After 3s error display       |
| ERROR       | [reboot]    | Watchdog timeout (unrecoverable)                  | —                            |
| SLEEP       | IDLE        | GPIO0 interrupt (button press)                    | —                            |
| SLEEP       | IDLE        | Timer wakeup (configurable, default 10 min)       | —                            |
| Any         | ERROR       | `event_bus_publish(EVENT_STORAGE_ERROR)` etc.     | Severity check               |

---

## 4. State Implementation

### 4.1 API (`state.h`)

```c
esp_err_t      state_init(void);
device_state_t state_get(void);
esp_err_t      state_set(device_state_t new_state);
const char*    state_to_string(device_state_t s);
```

### 4.2 Behavior on `state_set()`

1. If `new_state == current_state`, return `ESP_ERR_INVALID_STATE` (no re-broadcast).
2. Update internal state variable (mutex-protected).
3. Publish `EVENT_STATE_CHANGED` with `{ prev_state, curr_state }` payload.
4. All subscribers (ui, app_main, etc.) receive callback synchronously.

### 4.3 Subscribing to State Changes

```c
// In any module init:
event_bus_subscribe(EVENT_STATE_CHANGED, my_on_state_changed, NULL);

// Callback:
static void my_on_state_changed(event_type_t type, const void *data, size_t len, void *user) {
    const event_state_data_t *ev = (const event_state_data_t *)data;
    if (ev->curr_state == DEVICE_STATE_RECORDING) {
        // start recording pipeline
    }
}
```

---

## 5. LED Patterns per State

| State     | LED Pattern     | Frequency   |
|-----------|----------------|-------------|
| INIT      | White solid    | —           |
| IDLE      | Green slow blink| 1 Hz        |
| RECORDING | Red fast blink | 5 Hz        |
| UPLOADING | Blue breathing | ~0.5 Hz     |
| ERROR     | Red rapid blink| 10 Hz       |
| SLEEP     | Off            | —           |

---

## 6. Design Principles

1. **Single responsibility**: Each state has exactly one active activity.
2. **Event-driven transitions**: State changes triggered by events, not polling.
3. **No state polling**: Modules subscribe to `EVENT_STATE_CHANGED`. They do NOT call `state_get()` in a loop.
4. **Recoverable errors**: All error states have a defined recovery path back to IDLE.
5. **Atomic transitions**: `state_set()` is mutex-protected; no race conditions.

---

## 7. Future States (Planned)

| State          | When | Description                          |
|----------------|------|--------------------------------------|
| `PROVISIONING` | v0.6 | BLE/WiFi provisioning mode           |
| `OTA_UPDATE`   | v0.6 | Firmware OTA in progress             |
| `DIAGNOSTIC`   | —    | Factory self-test mode               |
