# Coding Style — ESP32 AI Recorder

> Version: v0.2 | Updated: 2026-05-12
> Framework: ESP-IDF v5.2 / C99 / FreeRTOS

---

## 1. Log Format

### 1.1 TAG Naming

Every source file defines a module-level log tag at the top:

```c
static const char *TAG = "component_name";
```

Tag must match the component directory name:

| File                          | TAG             |
|-------------------------------|-----------------|
| `components/audio/audio.c`    | `"audio"`       |
| `components/storage/storage.c`| `"storage"`     |
| `components/button/button.c`  | `"button"`      |
| `components/wifi_manager/wifi_manager.c` | `"wifi_mgr"` |
| `main/app_main.c`             | `"app_main"`    |

### 1.2 Log Levels

| Level        | Macro         | When to Use                                    |
|--------------|---------------|------------------------------------------------|
| Error        | `ESP_LOGE`    | Failures that block functionality              |
| Warning      | `ESP_LOGW`    | Unexpected conditions, degraded operation      |
| Info         | `ESP_LOGI`    | Normal operations, state changes, init success |
| Debug        | `ESP_LOGD`    | Periodic low-level data (disabled in release)  |
| Verbose      | `ESP_LOGV`    | Very detailed trace (almost never used)        |

### 1.3 Log Message Format

```c
// ✅ Good: TAG, verb, context
ESP_LOGI(TAG, "SD card mounted at %s (%.1f GB free)", mount_point, free_gb);
ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
ESP_LOGW(TAG, "WiFi not connected, skipping upload");

// ❌ Bad: no context, unclear
ESP_LOGI(TAG, "done");
ESP_LOGE(TAG, "error");
```

---

## 2. Naming Conventions

### 2.1 Functions

```c
// Format: <component>_<verb>[_<noun>]()
esp_err_t storage_mount(const char *mount_point);
void      storage_unmount(void);
esp_err_t storage_test_rw(void);
bool      storage_file_exists(const char *path);
int       storage_list_wav_files(const char *dir, char list[][64], int max);
```

- All public functions prefixed with component name
- Return `esp_err_t` for operations that can fail
- Return `void` for operations that cannot fail
- Static (internal) functions: no prefix required

### 2.2 Types and Enums

```c
// Structs: snake_case with _t suffix
typedef struct {
    int sample_rate;
    int bits_per_sample;
} recorder_config_t;

// Enums: UPPER_SNAKE_CASE
typedef enum {
    DEVICE_STATE_INIT = 0,
    DEVICE_STATE_IDLE,
    DEVICE_STATE_RECORDING,
} device_state_t;

// Enum values: MODULE_PREFIX_NAME
// ✅ DEVICE_STATE_RECORDING
// ❌ STATE_RECORDING, RECORDING
```

### 2.3 Constants and Macros

```c
// Macro constants: UPPER_SNAKE_CASE
#define PIN_NUM_MOSI    11
#define MAX_FILES       32
#define AUDIO_STACK_SIZE  8192

// Avoid magic numbers — always named constants
// ✅ host.max_freq_khz = SDMMC_FREQ_DEFAULT;
// ❌ host.max_freq_khz = 20000;
```

### 2.4 Variables

```c
// Local: snake_case
int bytes_read = 0;
esp_err_t ret;

// Static module-level: s_ prefix
static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

// Global (avoid!): g_ prefix if absolutely necessary
```

---

## 3. Task Naming

Format: `"component_task"` or `"component_verb"`

```c
// ✅ Good
xTaskCreate(audio_task,  "audio",        8192, NULL, 3, NULL);
xTaskCreate(upload_task, "upload",       4096, NULL, 2, NULL);
xTaskCreate(mon_task,    "sys_monitor",  2048, NULL, 1, NULL);

// ❌ Bad
xTaskCreate(task1, "Task1", 4096, NULL, 1, NULL);
xTaskCreate(func,  "t",     4096, NULL, 1, NULL);
```

---

## 4. Event Naming

Format: `EVENT_<NOUN>_<VERB>` (past tense for "has happened" events)

```c
// ✅ Good
EVENT_RECORDING_STARTED
EVENT_RECORDING_STOPPED
EVENT_UPLOAD_DONE
EVENT_UPLOAD_FAILED
EVENT_STORAGE_READY
EVENT_BUTTON_CLICKED
EVENT_STATE_CHANGED

// ❌ Bad
EVENT_START_RECORDING    // verb-first, ambiguous
EVENT_REC                // too short
EVENT_DO_UPLOAD          // imperative, sounds like a command not an event
```

---

## 5. State Naming

Format: `DEVICE_STATE_<NAME>`

```c
typedef enum {
    DEVICE_STATE_INIT = 0,
    DEVICE_STATE_IDLE,
    DEVICE_STATE_RECORDING,
    DEVICE_STATE_UPLOADING,
    DEVICE_STATE_ERROR,
    DEVICE_STATE_SLEEP,
    DEVICE_STATE_COUNT,   // always last, used for bounds checking
} device_state_t;
```

---

## 6. File Naming

| Type              | Convention          | Example                  |
|-------------------|---------------------|--------------------------|
| C source          | `snake_case.c`      | `storage.c`              |
| C header          | `snake_case.h`      | `storage.h`              |
| Test/script       | `snake_case.py/.sh` | `mock_upload_test.py`    |
| Documentation     | `kebab-case.md`     | `coding-style.md`        |
| Component dir     | `snake_case/`       | `wifi_manager/`          |

---

## 7. FreeRTOS Usage Principles

### 7.1 Task Creation

```c
// ✅ Use xTaskCreatePinnedToCore for audio/I2S (requires core affinity)
xTaskCreatePinnedToCore(
    audio_task,        // function
    "audio",           // name (max 16 chars)
    8192,              // stack size in bytes (NOT words)
    NULL,              // parameter
    3,                 // priority (1=low, 5=high, <24 to avoid WiFi conflicts)
    NULL,              // handle (NULL if not needed)
    0                  // core (0 or 1; tskNO_AFFINITY for don't care)
);
```

### 7.2 Priority Guidelines

| Priority | Use                                  |
|----------|--------------------------------------|
| 1        | Background tasks (monitor, logging)  |
| 2        | Upload, WiFi application tasks       |
| 3        | Audio capture (time-sensitive)       |
| 5        | Button debounce                      |
| 23+      | Reserved for ESP-IDF WiFi stack      |

Never use priority ≥ 24 (ESP-IDF WiFi tasks operate here).

### 7.3 Stack Sizes

```c
// Minimum practical sizes:
#define STACK_MIN_SIMPLE   2048   // simple tasks, no printf
#define STACK_MIN_LOGGING  3072   // tasks with ESP_LOG*
#define STACK_AUDIO        8192   // audio with math + I2S
#define STACK_UPLOAD       6144   // HTTP client
```

Always monitor with `system_monitor_init()` — check watermarks.
If watermark < 512 bytes, increase stack.

### 7.4 Synchronization

```c
// ✅ Prefer: FreeRTOS queues for task-to-task data passing
QueueHandle_t q = xQueueCreate(10, sizeof(audio_frame_t));
xQueueSend(q, &frame, portMAX_DELAY);

// ✅ OK: Mutex for shared resource protection
SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
xSemaphoreTake(mutex, portMAX_DELAY);
// ... access shared resource ...
xSemaphoreGive(mutex);

// ❌ Avoid: global variables shared across tasks without protection
// ❌ Avoid: polling in a tight loop (use vTaskDelay or semaphore)
// ❌ Avoid: blocking operations in interrupt handlers (ISR)
```

### 7.5 Delays

```c
// ✅ Always use pdMS_TO_TICKS for readable delays
vTaskDelay(pdMS_TO_TICKS(100));   // 100ms

// ❌ Never use raw tick counts
vTaskDelay(10);  // unclear and depends on configTICK_RATE_HZ
```

---

## 8. Module Boundary Principles

1. **HAL components** (`audio`, `led`, `button`, `storage`, `battery`) must NOT:
   - Know about business logic
   - Call state_set() or event_bus_publish() directly (except storage/battery which publish hardware events)
   - Import business component headers

2. **Business components** (`recorder`, `uploader`, `wifi_manager`) must NOT:
   - Access GPIO directly
   - Use hardware peripheral APIs directly (use HAL component APIs)

3. **Architecture components** (`event_bus`, `state`) must NOT:
   - Depend on any other custom component

4. **app_main** is the only place where:
   - All components are imported together
   - Initialization order is defined
   - Top-level event subscriptions are registered

---

## 9. Error Handling

```c
// ✅ Always check esp_err_t returns
esp_err_t ret = storage_mount("/sdcard");
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Storage mount failed: %s", esp_err_to_name(ret));
    // decide: return, fallback, or ESP_ERROR_CHECK
}

// ✅ Use ESP_ERROR_CHECK for "must succeed or crash" operations
ESP_ERROR_CHECK(state_init());
ESP_ERROR_CHECK(nvs_flash_init());

// ❌ Never ignore esp_err_t silently
storage_mount("/sdcard");  // no check — forbidden
```

---

## 10. Header Files

```c
// Standard order in .c files:
#include "component_own.h"     // own header first
#include "esp_log.h"           // ESP-IDF headers
#include "freertos/FreeRTOS.h" // FreeRTOS
#include "driver/i2s_std.h"    // peripheral drivers
#include <stdint.h>            // standard C headers last
#include <string.h>
```

Every header must have include guards:
```c
#ifndef COMPONENT_NAME_H
#define COMPONENT_NAME_H
// ...
#endif /* COMPONENT_NAME_H */
```
