# Event System — ESP32 AI Recorder

> **Version:** v0.1 | **Updated:** 2026-05-14
> **Scope:** Event bus design principles and event type registry

---

## 1. Architecture

The event bus is a **synchronous pub/sub** system:
- Any component can publish an event
- Any component can subscribe to any event
- All subscriber callbacks run **synchronously** before `event_bus_publish()` returns
- No event queuing — subscribers must be fast (no blocking in callbacks)

```
Component A ──publish──▶ event_bus ──dispatch──▶ Component B (callback)
                                   ├──dispatch──▶ Component C (callback)
                                   └──dispatch──▶ Component D (callback)
```

---

## 2. Event Type Registry

All event types are defined in `event_bus.h`. **Never use magic numbers** — always use the enum.

```c
typedef enum {
    /* State events */
    EVENT_STATE_CHANGED,              // Device state transitioned

    /* Button events */
    EVENT_BUTTON_PRESSED,             // GPIO pressed (debounced)
    EVENT_BUTTON_RELEASED,            // GPIO released
    EVENT_BUTTON_CLICKED,             // Single click (press + release)
    EVENT_BUTTON_DOUBLE_CLICKED,      // Double click within 300ms
    EVENT_BUTTON_LONG_PRESSED,        // Held for >1.5s
    EVENT_BUTTON_HOLD,                // Held continuously (every 500ms)

    /* Storage events */
    EVENT_STORAGE_READY,              // SD card mounted successfully
    EVENT_STORAGE_ERROR,              // SD card error (mount fail, write fail)

    /* Battery events */
    EVENT_BATTERY_LOW,               // Battery below 20%
    EVENT_BATTERY_CRITICAL,          // Battery below 5%

    /* WiFi events */
    EVENT_WIFI_CONNECTED,            // Connected to AP
    EVENT_WIFI_DISCONNECTED,         // Disconnected

    /* Recording events */
    EVENT_RECORDING_STARTED,         // Recording has begun
    EVENT_RECORDING_STOPPED,         // Recording has ended

    /* Upload events */
    EVENT_UPLOAD_STARTED,            // Upload began
    EVENT_UPLOAD_PROGRESS,           // Upload progress update (0–100)
    EVENT_UPLOAD_DONE,               // Upload completed successfully
    EVENT_UPLOAD_FAILED,             // Upload failed

    /* Total — must be last */
    EVENT_COUNT,
} event_type_t;
```

---

## 3. Event Payloads

Each event may carry a typed payload. Always cast to the correct struct:

| Event | Payload Type | Fields |
|-------|-------------|--------|
| `EVENT_STATE_CHANGED` | `event_state_data_t` | `prev_state`, `curr_state` |
| `EVENT_BUTTON_*` | `event_button_data_t` | `gpio_num`, `duration_ms` |
| `EVENT_UPLOAD_PROGRESS` | `event_upload_progress_t` | `bytes_sent`, `bytes_total` |
| All others | `NULL` | — |

---

## 4. Subscription Rules

### 4.1 Subscribe at Init Time

All subscriptions are made during component init, before `state_set(IDLE)` is called:

```c
// In my_component_init():
event_bus_subscribe(EVENT_STATE_CHANGED, on_state_changed, NULL);
event_bus_subscribe(EVENT_BUTTON_CLICKED, on_button, NULL);
```

### 4.2 Callback Signature

```c
static void my_callback(event_type_t type, const void *data, size_t len, void *user) {
    (void)type; (void)len; (void)user;

    switch (type) {
    case EVENT_STATE_CHANGED: {
        const event_state_data_t *ev = (const event_state_data_t *)data;
        if (ev->curr_state == DEVICE_STATE_RECORDING) {
            // handle recording start
        }
        break;
    }
    // ... other events
    }
}
```

### 4.3 No Blocking in Callbacks

Callbacks run synchronously in the publishing task's context. **Never block** in a callback:
- No `vTaskDelay`, `vTaskSuspend`
- No `fopen`, `fwrite`, `fclose`
- No `esp_http_client_*` calls
- No `xSemaphoreTake` with long waits

If a callback needs to do heavy work, post to a queue and handle in a separate task.

---

## 5. Publish Rules

### 5.1 Publish from Task Context Only

Never publish from an ISR (interrupt service routine). Use `esp_intr_dump()` or a deferred handler if needed.

### 5.2 Include Typed Payloads

```c
event_state_data_t ev = {
    .prev_state = prev,
    .curr_state = curr,
};
event_bus_publish(EVENT_STATE_CHANGED, &ev, sizeof(ev));
```

### 5.3 Always Publish State Changes Through `state_set()`

Do NOT publish `EVENT_STATE_CHANGED` directly. Always call `state_set()` which handles:
1. Validating the transition
2. Updating internal state
3. Publishing the event with correct payload

---

## 6. Recording-Specific Events

### EVENT_RECORDING_STARTED

Published when `recorder_start()` is called and the WAV file is open.

**Payload:** `event_recording_data_t`
```c
typedef struct {
    const char *filepath;    // Full path to the recording file
    uint32_t session_id;     // Monotonic session number
} event_recording_data_t;
```

### EVENT_RECORDING_STOPPED

Published when `recorder_stop()` completes and the file is finalized.

**Payload:** `event_recording_data_t`
```c
typedef struct {
    const char *filepath;    // Full path to the completed file
    uint32_t session_id;     // Same session_id as STARTED
    uint32_t duration_ms;    // Recording duration in ms
    uint32_t file_size_bytes;// Final file size
} event_recording_data_t;
```

---

## 7. Event Bus Capacity

The event bus has 32 subscription slots. If all slots are full, `event_bus_subscribe()` returns `ESP_ERR_NO_MEM`.

Current usage:
- `ui.c`: subscribes to STATE_CHANGED, STORAGE_*, BATTERY_*
- `app_main.c`: subscribes to BUTTON_*, STATE_CHANGED
- `recorder.c`: subscribes to STATE_CHANGED (for recording events)
- `uploader.c`: subscribes to RECORDING_STOPPED, UPLOAD_*

---

*Last updated: 2026-05-14*
